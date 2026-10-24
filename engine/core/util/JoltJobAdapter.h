// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOLTJOBADAPTER_H
#define JOLTJOBADAPTER_H

#ifdef JASPERENGINE_PHYSICS_3D

#include "Jolt/Jolt.h"
#include "Jolt/Core/FixedSizeFreeList.h"
#include "Jolt/Core/JobSystemWithBarrier.h"

#include <atomic>

namespace jasperengine {

    // Jolt's physics jobs on the engine's job system (High lane), so physics and the engine
    // share one CPU budget instead of Jolt starting a pool of its own. Barriers come from
    // JobSystemWithBarrier: the thread that waits on one runs its jobs too. In single-thread
    // mode a job runs as soon as it can.
    class JoltJobAdapter final : public JPH::JobSystemWithBarrier {
    public:
        JoltJobAdapter(JPH::uint maxJobs, JPH::uint maxBarriers);
        // Waits for the queued envelopes still holding a job: a barrier waiter may have run a job
        // already, but its envelope releases it later, into this adapter's pool
        ~JoltJobAdapter() override;

        int GetMaxConcurrency() const override;
        JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 numDependencies = 0) override;

    protected:
        void QueueJob(Job* job) override;
        void QueueJobs(Job** list, JPH::uint count) override;
        void FreeJob(Job* job) override;

    private:
        using AvailableJobs = JPH::FixedSizeFreeList<Job>;
        AvailableJobs jobPool;
        // envelopes queued on the job system and not finished yet
        std::atomic<int> queuedEnvelopes{0};
    };

}

#endif // JASPERENGINE_PHYSICS_3D

#endif // JOLTJOBADAPTER_H
