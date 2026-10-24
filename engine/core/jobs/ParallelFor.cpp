// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's ParallelForCore.cpp (Modules/JobSystem/Source/ParallelForCore.cpp),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.
// Changes: our scheduler and slabs, a grain, parking on the job system's parking lot
// (mutex + condition variable) instead of std::atomic::wait, and helpers that inherit the
// lane and gate class of the forking body.
//
// One run is a RunBlock in a 128-byte slab, shared by the caller and its helpers through an
// intrusive reference count; claims (of `grain` units each) are taken one at a time from one
// atomic word, and the caller joins on the count of claims in progress only.
//
// The claim word: the next claim in the low bits, kClosed in the high bit. A claim is one
// seq_cst fetch_add(1), which takes a claim and observes the closure in the same
// read-modify-write; closing (the caller, at the end of its loop) and stopping (a false body,
// a throw, the stop poll) are a seq_cst fetch_or(kClosed) on the same word, so no claim
// starts after a stop in the word's modification order.
//
// A helper, in order:
//  (0) load the claim word: closed or past the end -> drop the reference and end, having
//      counted nothing and touched nothing of the caller's;
//  (1) count itself in progress;
//  (2) re-load the claim word: closed -> leave;
//  (3) claim and run until nothing is left or the run is closed, then leave.
// Leave: decrement in-progress; if that reached zero and the claim word shows closed, wake
// the caller. Two Dekker pairs make the join exact: the helper's increment then claim-word
// load against the caller's close then in-progress load (either the helper backs out at (2)
// or the caller sees it in flight), and the helper's decrement then claim-word load against
// the same close then load (either the helper sees closed and wakes the caller, or the
// caller sees zero and never waits). The caller's body is touched only between an open
// observation at (2) and the leave, and the caller returns only once the count is zero
// after its close: a helper running after the caller returned touches only the block,
// which its own reference keeps alive.

#include "jobs/JobSystem.h"
#include "jobs/detail/Scheduler.h"

#include "thread/ThreadUtil.h"
#include "jobs/detail/TimedWait.h"

#include <algorithm>
#include <chrono>
#include <exception>

using namespace jasperengine;
using namespace jasperengine::jobs;
using namespace jasperengine::jobs::detail;

namespace{

    constexpr size_t kClosed = size_t(1) << (sizeof(size_t) * 8 - 1);

    struct RunBlock {
        std::atomic<size_t> claim{0};
        std::atomic<uint32_t> inProgress{0};
        std::atomic<uint32_t> references{0};
        std::atomic<uint32_t> parked{0};
        std::atomic<bool> stopped{false};
        std::atomic<bool> hasException{false};
        std::exception_ptr exception;
        size_t claimCount = 0;
        size_t grain = 1;
        size_t count = 0;
        ParallelForInvoke invoke = nullptr;
        void* body = nullptr;
        Scheduler* scheduler = nullptr;
        CancelState* cancel = nullptr;
        Lane lane = Lane::Normal;
        GateClass gate = GateClass::Default;
    };
    static_assert(sizeof(RunBlock) <= kJobSlabBytes, "a parallelFor run block fits one job slab");

    void dropReference(RunBlock* block){
        if (block->references.fetch_sub(1, std::memory_order_acq_rel) == 1){
            block->~RunBlock();
            releaseSlab(SlabClass::Job, block);
        }
    }

    bool isOpenWithClaimsLeft(size_t word, size_t claimCount){
        return (word & kClosed) == 0 && word < claimCount;
    }

    void stopRun(RunBlock& block){
        block.stopped.store(true, std::memory_order_relaxed);
        block.claim.fetch_or(kClosed, std::memory_order_seq_cst);
    }

    // Keeps the first exception of the run and stops it. Called inside a catch.
    void recordExceptionAndStop(RunBlock& block){
        if (!block.hasException.exchange(true, std::memory_order_acq_rel))
            block.exception = std::current_exception();
        stopRun(block);
    }

    void runClaim(RunBlock& block, size_t claim){
        const size_t begin = claim * block.grain;
        const size_t end = std::min(begin + block.grain, block.count);
        try{
            if (!block.invoke(block.body, begin, end))
                stopRun(block);
        }catch(...){
            recordExceptionAndStop(block);
        }
    }

    void leave(RunBlock& block){
        if (block.inProgress.fetch_sub(1, std::memory_order_seq_cst) == 1
            && (block.claim.load(std::memory_order_seq_cst) & kClosed) != 0
            && block.parked.load(std::memory_order_seq_cst) > 0){
            std::lock_guard<std::mutex> lock(bucketMutex(&block));
            bucketCv(&block).notify_all();
        }
    }

    // A helper job: one reference to the block, and one count of the lane's unstarted
    // helpers, given back when it starts (or when it is destroyed unrun).
    class ClaimHelper {
    public:
        ClaimHelper(RunBlock* block, std::atomic<uint32_t>* unstarted): block(block), unstarted(unstarted) {}
        ClaimHelper(ClaimHelper&& other) noexcept: block(other.block), unstarted(other.unstarted){
            other.block = nullptr;
            other.unstarted = nullptr;
        }
        ClaimHelper(const ClaimHelper&) = delete;
        ClaimHelper& operator=(const ClaimHelper&) = delete;
        ClaimHelper& operator=(ClaimHelper&&) = delete;
        ~ClaimHelper(){ reset(); }

        void operator()(){
            giveBackUnstarted();
            RunBlock& run = *block;
            // (0)
            if (!isOpenWithClaimsLeft(run.claim.load(std::memory_order_acquire), run.claimCount)){
                reset();
                return;
            }
            // (1)
            run.inProgress.fetch_add(1, std::memory_order_seq_cst);
            // (2)
            if ((run.claim.load(std::memory_order_seq_cst) & kClosed) != 0){
                leave(run);
                reset();
                return;
            }
            // (3) the caller's cancel token is visible to the units
            BodyContext& body = currentBody();
            CancelState* savedCancel = body.cancel;
            body.cancel = run.cancel;
            for (;;){
                const size_t claim = run.claim.fetch_add(1, std::memory_order_seq_cst);
                if (!isOpenWithClaimsLeft(claim, run.claimCount))
                    break;
                runClaim(run, claim);
            }
            body.cancel = savedCancel;
            leave(run);
            reset();
        }

    private:
        void giveBackUnstarted(){
            if (unstarted){
                unstarted->fetch_sub(1, std::memory_order_relaxed);
                unstarted = nullptr;
            }
        }

        void reset(){
            giveBackUnstarted();
            if (block){
                dropReference(block);
                block = nullptr;
            }
        }

        RunBlock* block;
        std::atomic<uint32_t>* unstarted;
    };

    // The caller's join: until no claim is in progress. After the caller closed the run.
    void waitForClaimsInProgress(RunBlock& block){
        if (block.inProgress.load(std::memory_order_seq_cst) == 0)
            return;
        WaitKind kind = WaitKind::ParallelFor;
        const ProfilerHooks* h = hooks();
        if (h && h->onWaitBegin)
            h->onWaitBegin(kind);
        if (thread::canBlock()){
            std::unique_lock<std::mutex> lock(bucketMutex(&block));
            block.parked.fetch_add(1, std::memory_order_seq_cst);
            while (block.inProgress.load(std::memory_order_seq_cst) != 0)
                timedWait(bucketCv(&block), lock, std::chrono::milliseconds(10));
            block.parked.fetch_sub(1, std::memory_order_seq_cst);
        }else{
            // the browser main thread: a unit in progress may need its proxied calls
            while (block.inProgress.load(std::memory_order_seq_cst) != 0)
                spinWait();
        }
        h = hooks();
        if (h && h->onWaitEnd)
            h->onWaitEnd();
    }

    void runCallerClaims(RunBlock& block, const std::function<bool()>* stop){
        for (;;){
            if (stop && *stop){
                bool stopRequested = false;
                try{
                    stopRequested = (*stop)();
                }catch(...){
                    recordExceptionAndStop(block);
                    return;
                }
                if (stopRequested){
                    stopRun(block);
                    return;
                }
            }
            const size_t claim = block.claim.fetch_add(1, std::memory_order_seq_cst);
            if (!isOpenWithClaimsLeft(claim, block.claimCount))
                return;
            runClaim(block, claim);
        }
    }

    // Every exit of a run with helpers closes it and waits for the claims in progress
    // before the caller's frame unwinds: helpers write into it.
    class CloseAndJoinOnExit {
    public:
        explicit CloseAndJoinOnExit(RunBlock& block): block(block) {}
        ~CloseAndJoinOnExit(){
            block.claim.fetch_or(kClosed, std::memory_order_seq_cst);
            waitForClaimsInProgress(block);
        }
        CloseAndJoinOnExit(const CloseAndJoinOnExit&) = delete;
        CloseAndJoinOnExit& operator=(const CloseAndJoinOnExit&) = delete;

    private:
        RunBlock& block;
    };

    class CallerReference {
    public:
        explicit CallerReference(RunBlock* block): block(block) {}
        ~CallerReference(){ dropReference(block); }
        CallerReference(const CallerReference&) = delete;
        CallerReference& operator=(const CallerReference&) = delete;

    private:
        RunBlock* block;
    };

    // A run with no helper: every claim on the caller. The first throw leaves at once.
    bool runEveryClaimOnTheCaller(ParallelForInvoke invoke, void* body, size_t count, size_t grain,
                                  const std::function<bool()>* stop){
        for (size_t begin = 0; begin < count; begin += grain){
            if (stop && *stop && (*stop)())
                return false;
            if (!invoke(body, begin, std::min(begin + grain, count)))
                return false;
        }
        return true;
    }

    // Reserves up to `wanted` of the lane's unstarted-helper budget.
    size_t reserveHelpers(std::atomic<uint32_t>& unstarted, size_t wanted, size_t limit){
        uint32_t current = unstarted.load(std::memory_order_relaxed);
        for (;;){
            if (current >= limit)
                return 0;
            const size_t take = std::min(wanted, limit - current);
            if (unstarted.compare_exchange_weak(current, (uint32_t)(current + take), std::memory_order_relaxed))
                return take;
        }
    }

    // The caller's units are job bodies whichever thread runs them.
    class CallerBody {
    public:
        CallerBody(){
            BodyContext& body = currentBody();
            wasInBody = body.inBody;
            body.inBody = true;
        }
        ~CallerBody(){ currentBody().inBody = wasInBody; }

    private:
        bool wasInBody;
    };
}

bool jobs::detail::runParallelFor(Scheduler* scheduler, ParallelForInvoke invoke, void* body, size_t count,
                            const ParallelForOptions& options){
    if (count == 0)
        return true;
    const size_t grain = std::max<size_t>(options.grain, 1);
    const size_t claimCount = (count + grain - 1) / grain;
    const CallerBody callerBody;

    const size_t workers = (size_t)scheduler->workerCount;
    const size_t asked = options.maxHelpers == ParallelForOptions::kAllWorkers ? workers : options.maxHelpers;
    const size_t wanted = (scheduler->singleThread || claimCount <= 1
                           || scheduler->shuttingDown.load(std::memory_order_acquire))
        ? 0 : std::min({asked, claimCount - 1, kMaxHelpersPerRun});

    const BodyContext& context = currentBody();
    const Lane lane = scheduler->clampLane(options.lane);
    const GateClass gateClass = (lane == Lane::Background && context.scheduler && context.lane == Lane::Background)
        ? context.gate : GateClass::Default;
    std::atomic<uint32_t>& unstarted = scheduler->unstartedHelpers[(size_t)lane];
    const size_t helpers = wanted == 0 ? 0
        : reserveHelpers(unstarted, wanted, (size_t)kMaxUnstartedHelpersPerWorker * std::max<size_t>(workers, 1));
    if (helpers == 0)
        return runEveryClaimOnTheCaller(invoke, body, count, grain, options.stop);

    auto* block = ::new (acquireSlab(SlabClass::Job)) RunBlock();
    block->claimCount = claimCount;
    block->grain = grain;
    block->count = count;
    block->invoke = invoke;
    block->body = body;
    block->scheduler = scheduler;
    block->cancel = context.cancel;
    block->lane = lane;
    block->gate = gateClass;
    // the caller's reference and one per helper, before any helper can run and drop its own
    block->references.store((uint32_t)(helpers + 1), std::memory_order_relaxed);
    const CallerReference reference(block);

    {
        const CloseAndJoinOnExit join(*block);
        {
            Job* staged[kMaxHelpersPerRun];
            size_t made = 0;
            try{
                for (; made < helpers; made++)
                    staged[made] = makeJob(ClaimHelper(block, &unstarted), lane, gateClass, "parallel-for", 0);
            }catch(...){
                // the helpers never made give their reservation and references back
                for (size_t i = made; i < helpers; i++){
                    unstarted.fetch_sub(1, std::memory_order_relaxed);
                    dropReference(block);
                }
                scheduler->publishBatch(staged, made, lane, gateClass);
                throw;
            }
            scheduler->publishBatch(staged, made, lane, gateClass);
        }
        runCallerClaims(*block, options.stop);
    }

    const bool stopped = block->stopped.load(std::memory_order_relaxed);
    if (block->hasException.load(std::memory_order_acquire))
        std::rethrow_exception(block->exception);
    return !stopped;
}
