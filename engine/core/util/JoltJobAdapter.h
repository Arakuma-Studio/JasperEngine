// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOLTJOBADAPTER_H
#define JOLTJOBADAPTER_H

#ifdef JASPERENGINE_PHYSICS_3D

#include "jobs/JobCounter.h"

#include "Jolt/Jolt.h"
#include "Jolt/Core/FixedSizeFreeList.h"
#include "Jolt/Core/JobSystemWithBarrier.h"

namespace jasperengine {

    // Jolt's physics jobs on the engine's job system (High lane), so physics and the engine
    // share one CPU budget instead of Jolt starting a pool of its own. Barriers come from
    // JobSystemWithBarrier: the thread that waits on one runs its jobs too. In single-thread
    // mode a job runs as soon as it can.
    class JoltJobAdapter final : public JPH::JobSystemWithBarrier {
    public:
        JoltJobAdapter(JPH::uint maxJobs, JPH::uint maxBarriers);
        // Waits for the envelopes still queued (running the ones no worker took): each holds a
        // job of this adapter and frees it through FreeJob
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
        // the queued envelopes, joined by the destructor
        jobs::JobCounter envelopes;
    };

}

#endif // JASPERENGINE_PHYSICS_3D

#endif // JOLTJOBADAPTER_H
